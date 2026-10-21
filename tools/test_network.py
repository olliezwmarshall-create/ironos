#!/usr/bin/env python3
"""Exercise Ethernet/DHCP/ARP/ICMP/DNS using disposable QEMU user networking."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_install import VM


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--qemu', default='qemu-system-x86_64')
    ap.add_argument('--firmware', action='append', default=[])
    ap.add_argument('--internet', action='store_true', help='also require a public DNS lookup')
    ap.add_argument('--nic',choices=['rtl8139','e1000'],default='rtl8139')
    args = ap.parse_args()
    args.network = f'-netdev user,id=net0 -device {args.nic},id=nic0,netdev=net0,romfile='
    with tempfile.TemporaryDirectory(prefix='ironos-network-', dir='/tmp') as tmp:
        work = Path(tmp)
        with (work / 'target.img').open('wb') as f:
            f.truncate(128 * 1024 * 1024)
        subprocess.run(['python3', 'mkdisk.py', str(work / 'data.img'), 'disk'], check=True)
        vm = VM(args, work)
        try:
            vm.expect('kernel:/$>')
            vm.send('net'); vm.expect('Ethernet: '+('Intel E1000' if args.nic=='e1000' else 'Realtek RTL8139'))
            vm.expect('IPv4: not configured')
            vm.send('ping 10.0.2.2'); vm.expect('No IPv4 lease')
            vm.send('dhcp'); vm.expect('DHCP lease acquired: 10.0.2.15')
            vm.send('net'); vm.expect('Gateway: 10.0.2.2  DNS: 10.0.2.3')
            vm.send('ping 10.0.2.2'); vm.expect('3/3 replies')
            vm.send('netoff'); vm.expect('configuration cleared')
            vm.send('netconfig 10.0.2.15 255.255.255.0 10.0.2.2 10.0.2.3'); vm.expect('Static IPv4 configured')
            vm.send('net'); vm.expect('Static IPv4 configuration')
            vm.send('ping 10.0.2.2'); vm.expect('3/3 replies')
            # Enough probes to cross the 8 KiB receive-ring boundary repeatedly.
            for _ in range(48):
                vm.send('clear')
                vm.send('ping 10.0.2.2'); vm.expect('3/3 replies')
            if args.internet:
                vm.send('resolve example.com'); print(vm.expect('example.com = '))
            vm.send('ping 0.0.0.0'); vm.expect('Use a unicast remote IPv4 address')
            vm.send('ping 10.0.2.254'); vm.expect('ARP lookup failed', timeout=10)
            vm.qmp('set_link', {'name': 'nic0', 'up': False})
            vm.send('net'); vm.expect('Link: down')
            vm.send('dhcp'); vm.expect('Ethernet link down')
            vm.qmp('set_link', {'name': 'nic0', 'up': True})
            vm.send('clear'); vm.send('dhcp'); vm.expect('DHCP lease acquired: 10.0.2.15')
            vm.send('clear'); vm.send('ping 10.0.2.2'); vm.expect('3/3 replies')
            vm.send('memtest'); vm.expect('memtest: PASSED')
        finally:
            vm.close()
        args.network = ''
        vm = VM(args, work)
        try:
            vm.expect('kernel:/$>')
            vm.send('net'); vm.expect('No supported Ethernet card')
            vm.send('clear'); vm.send('dhcp'); vm.expect('No supported Ethernet card')
        finally:
            vm.close()
    print('PASS: NIC discovery, DHCP, ARP, repeated ICMP/ring wrap, timeout,')
    print('      link loss/recovery, memory test and missing-NIC handling')
    if args.internet:
        print('PASS: public DNS lookup through the NAT resolver')


if __name__ == '__main__':
    main()
