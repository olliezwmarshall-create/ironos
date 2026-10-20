#!/usr/bin/env python3
"""Independent local Ethernet peer for deterministic DHCP, DNS and inbound ICMP tests.
The TCP socket transports Ethernet frames to QEMU; it is not guest TCP support.
Uses only loopback and reserved test addresses; no public resolver is required.
"""
import argparse
import ipaddress
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time
from test_install import VM

MAC = bytes.fromhex('020000000002')
IP = ipaddress.IPv4Address
GUEST = IP('198.18.0.10').packed
ROUTER = IP('198.18.0.1').packed
DNS = IP('198.18.0.53').packed
ANSWER = IP('198.51.100.42').packed


def checksum(data):
    if len(data) & 1:
        data += b'\0'
    total = sum(struct.unpack('!' + 'H' * (len(data) // 2), data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535


def ip_packet(source, dest, protocol, data):
    header = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(data), 1, 0x4000, 64, protocol, 0, source, dest)
    header = header[:10] + struct.pack('!H', checksum(header)) + header[12:]
    return header + data


def udp_packet(source, dest, sport, dport, data):
    length = 8 + len(data)
    udp = struct.pack('!HHHH', sport, dport, length, 0) + data
    pseudo = source + dest + struct.pack('!BBH', 0, 17, length)
    udp = udp[:6] + struct.pack('!H', checksum(pseudo + udp) or 65535) + udp[8:]
    return ip_packet(source, dest, 17, udp)


class Peer:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.port = self.listener.getsockname()[1]
        self.listener.listen(1)
        self.listener.settimeout(.2)
        self.stop = False
        self.error = None
        self.conn = None
        self.client = None
        self.lock = threading.Lock()
        self.arp_reply = threading.Event()
        self.echo_reply = threading.Event()
        self.dns_queries = 0
        self.drop_dhcp = False
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def send(self, data):
        with self.lock:
            self.conn.sendall(struct.pack('!I', len(data)) + data)

    def ethernet(self, protocol, data, to=None):
        self.send((to or self.client) + MAC + struct.pack('!H', protocol) + data)

    def receive(self, data):
        if len(data) < 14:
            return
        self.client = data[6:12]
        protocol = struct.unpack_from('!H', data, 12)[0]
        payload = data[14:]
        if protocol == 0x806 and len(payload) >= 28:
            op = struct.unpack_from('!H', payload, 6)[0]
            if op == 2 and payload[14:18] == GUEST:
                self.arp_reply.set()
            if op == 1 and payload[24:28] in (ROUTER, DNS):
                arp = struct.pack('!HHBBH', 1, 0x800, 6, 4, 2) + MAC + payload[24:28] + self.client + payload[14:18]
                self.ethernet(0x806, arp)
            return
        if protocol != 0x800 or len(payload) < 20:
            return
        hlen = (payload[0] & 15) * 4
        total = struct.unpack_from('!H', payload, 2)[0]
        assert checksum(payload[:hlen]) == 0, 'Guest sent an invalid IPv4 checksum'
        body = payload[hlen:total]
        source, dest = payload[12:16], payload[16:20]
        if payload[9] == 1 and len(body) >= 8:
            assert checksum(body) == 0, 'Guest sent an invalid ICMP checksum'
            if body[0] == 0 and body[4:6] == b'\x12\x34':
                self.echo_reply.set()
            if body[0] == 8:
                reply = b'\0\0\0\0' + body[4:]
                reply = reply[:2] + struct.pack('!H', checksum(reply)) + reply[4:]
                self.ethernet(0x800, ip_packet(dest, source, 1, reply))
            return
        if payload[9] != 17 or len(body) < 8:
            return
        sport, dport, length, csum = struct.unpack_from('!HHHH', body)
        if csum:
            assert checksum(source + dest + struct.pack('!BBH', 0, 17, length) + body[:length]) == 0, 'Bad guest UDP checksum'
        data = body[8:length]
        if (sport, dport) == (68, 67):
            if self.drop_dhcp:
                return
            assert len(data) >= 240
            options, at = {}, 240
            while at < len(data):
                kind = data[at]; at += 1
                if kind == 255:
                    break
                if not kind:
                    continue
                n = data[at]; at += 1
                options[kind] = data[at:at+n]; at += n
            message = 2 if options.get(53) == b'\x01' else 5
            if message == 5:
                assert options.get(50) == GUEST and options.get(54) == ROUTER
            reply = bytearray(240)
            reply[:4] = b'\x02\x01\x06\x00'
            reply[4:8] = data[4:8]
            reply[16:20] = GUEST
            reply[28:34] = self.client
            reply[236:240] = bytes.fromhex('63825363')
            opts = bytes([53, 1, message, 54, 4]) + ROUTER
            opts += bytes([1, 4, 255, 255, 255, 0, 3, 4]) + ROUTER
            opts += bytes([6, 4]) + DNS + bytes([51, 4]) + struct.pack('!I', 600) + b'\xff'
            self.ethernet(0x800, udp_packet(ROUTER, b'\xff'*4, 67, 68, bytes(reply) + opts), b'\xff'*6)
        elif dport == 53:
            self.dns_queries += 1
            assert dest == DNS and data[12:] == b'\x07example\x04test\x00\x00\x01\x00\x01'
            response = data[:2] + struct.pack('!HHHHH', 0x8180, 1, 1, 0, 0) + data[12:]
            response += b'\xc0\x0c' + struct.pack('!HHIH', 1, 1, 60, 4) + ANSWER
            self.ethernet(0x800, udp_packet(DNS, GUEST, 53, sport, response))

    def run(self):
        try:
            while not self.stop:
                try:
                    self.conn, _ = self.listener.accept()
                    break
                except socket.timeout:
                    pass
            if self.stop:
                return
            self.conn.settimeout(.2)
            pending = b''
            while not self.stop:
                try:
                    chunk = self.conn.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                pending += chunk
                while len(pending) >= 4:
                    n = struct.unpack_from('!I', pending)[0]
                    assert n <= 65536
                    if len(pending) < n + 4:
                        break
                    frame, pending = pending[4:4+n], pending[4+n:]
                    self.receive(frame)
        except Exception as exc:
            self.error = exc

    def inbound_checks(self):
        arp = struct.pack('!HHBBH', 1, 0x800, 6, 4, 1) + MAC + ROUTER + b'\0'*6 + GUEST
        self.ethernet(0x806, arp, b'\xff'*6)
        assert self.arp_reply.wait(3), 'Guest did not answer an ARP request'
        echo = struct.pack('!BBHHH', 8, 0, 0, 0x1234, 1) + b'fixture'
        echo = echo[:2] + struct.pack('!H', checksum(echo)) + echo[4:]
        self.ethernet(0x800, ip_packet(ROUTER, GUEST, 1, echo))
        assert self.echo_reply.wait(3), 'Guest did not answer an ICMP request while idle'

    def close(self):
        self.stop = True
        self.thread.join(timeout=2)
        if self.conn:
            self.conn.close()
        self.listener.close()
        if self.error:
            raise self.error


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--qemu', default='qemu-system-x86_64')
    ap.add_argument('--firmware', action='append', default=[])
    args = ap.parse_args()
    peer = Peer()
    args.network = f'-netdev socket,id=net0,connect=127.0.0.1:{peer.port} -device rtl8139,id=nic0,netdev=net0,romfile='
    try:
        with tempfile.TemporaryDirectory(prefix='ironos-net-peer-', dir='/tmp') as tmp:
            work = Path(tmp)
            with (work / 'target.img').open('wb') as f:
                f.truncate(128 * 1024 * 1024)
            subprocess.run(['python3', 'mkdisk.py', str(work / 'data.img'), 'disk'], check=True)
            vm = VM(args, work)
            try:
                vm.expect('kernel:/$>')
                vm.send('dhcp'); vm.expect('DHCP lease acquired: 198.18.0.10')
                vm.send('resolve example.test'); vm.expect('example.test = 198.51.100.42')
                vm.send('ping example.test'); vm.expect('3/3 replies')
                assert peer.dns_queries == 2, 'DNS was not queried for both commands'
                vm.send('desktop'); vm.expect('IronOS Desktop')
                peer.inbound_checks()
                vm.hmp('sendkey t'); time.sleep(.2)
                vm.send('memtest'); vm.expect('memtest: PASSED')
                peer.drop_dhcp = True
                vm.send('clear'); vm.send('dhcp'); vm.expect('DHCP failed', timeout=18)
                vm.send('net'); vm.expect('IPv4: not configured')
                peer.drop_dhcp = False
                vm.send('clear'); vm.send('dhcp'); vm.expect('DHCP lease acquired: 198.18.0.10')
                peer.drop_dhcp = True
                vm.send('clear'); vm.send('dhcp'); vm.hmp('sendkey esc')
                vm.expect('DHCP failed', timeout=3)
            finally:
                vm.close()
    finally:
        peer.close()
    print('PASS: independently generated DHCP, checksummed/compressed DNS, hostname ping,')
    print('      off-subnet routing, inbound ARP/ICMP, DHCP timeout/cancel/recovery and memory test')


if __name__ == '__main__':
    main()
