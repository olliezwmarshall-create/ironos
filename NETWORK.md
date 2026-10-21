# Ethernet networking

IronOS automatically selects a supported PCI Ethernet controller: **Intel
82540EM (8086:100E), 82545EM (8086:100F)**, or **Realtek RTL8139
(10EC:8139)**. Intel is preferred if both are present; one interface is active.
It has a small IPv4 stack:
ARP, DHCP, UDP DNS lookup and ICMP ping/replies. The driver uses the legacy
RTL8139 datapath, including the compatible interface in QEMU's RTL8139C+.
This is the first networking layer; TCP, HTTP/HTTPS, sockets, Wi-Fi and USB
network adapters are not implemented. Other Ethernet cards need their own
drivers. PCI discovery alone does not make a card usable.

## WSL and QEMU

`make run` and `make run-installed` now provide an RTL8139 card through QEMU's
user-mode NAT. This needs no administrator networking setup and uses the host's
connection, including when Windows itself connects by Wi-Fi. It exposes a
**wired Ethernet card to IronOS**, not the host's physical Wi-Fi adapter.
The existing QEMU executable detection and flash-drive image workflow remain.

Inside IronOS:

```text
net
dhcp
net
ping 10.0.2.2
resolve example.com
ping example.com
```

`dhcp` obtains the address,
subnet mask, gateway and DNS server from the network. QEMU's default lease is
usually `10.0.2.15`, with gateway `10.0.2.2` and DNS proxy `10.0.2.3`.
No address is hard-coded in the stack. `net` reports link, MAC, configuration,
remaining lease time and packet counters. `ping` sends three probes; DNS
supports IPv4 answers and compressed DNS names, with bounded UDP-only replies.
Both numeric addresses and host names work with ping.

A successful gateway ping confirms the local virtual network. Public ping may
be filtered even when DNS/internet routing works. Web browsing and game server
connections need TCP/application support that is not present yet.

Press Escape to cancel a network command. DHCP, ARP and DNS have bounded
retries/timeouts. Leases expire and disable the old configuration; run `dhcp`
again to renew. Automatic lease renewal, IPv6, VLANs,
IP fragmentation/reassembly and DNS-over-TCP are not implemented.

The driver polls in the terminal, desktop, editor and 3D demo. Incoming ARP
requests and ping requests are answered while those event loops run. Long
blocking disk operations can delay packets. The receive ring detects and recovers
from malformed hardware lengths and overflow. Packet parsing checks lengths,
IPv4/ICMP checksums and nonzero UDP checksums before using the payload. Packet
counters count received/transmitted frames and driver/filter/transmit drops;
unsupported or invalid higher-level packets can also be silently discarded.

To disable networking entirely:

```sh
make run QEMU_NETWORK=
```

For a real PC, connect a cable and run `net`. If there is no supported card,
run `netcards` or `lspci` and note the **vendor:device ID** on the Ethernet/network line.
That identifies which driver to add next. Real hardware has not been validated.
Do not assume an adapter works merely because its PCI class says Ethernet.

## Network settings

`netcards` lists PCI network controllers, their IDs and driver availability.
Unknown controllers are reported but cannot carry packets. There is no
universal Wi-Fi/Ethernet/USB driver: Wi-Fi also needs firmware, association
and authentication; USB adapters need a USB host stack and chipset drivers.
Neither Wi-Fi nor USB networking is implemented in this version.

`netauto` obtains a DHCP lease (alias of `dhcp`). This is an explicit command,
not background DHCP at boot. `netoff` clears IPv4 configuration; it does not
power off the card or remove its driver. Static settings last until reboot
or the next DHCP/netoff command:

```text
netconfig 10.0.2.15 255.255.255.0 10.0.2.2 10.0.2.3
```

Arguments are IP, subnet mask, gateway and DNS. A gateway or DNS of `0.0.0.0`
omits that setting. Invalid settings preserve the current configuration.
The Network desktop app shows live settings and offers DHCP/disconnect.

Test Intel Ethernet in WSL with `make run QEMU_NIC=e1000`; ordinary `make run`
keeps the existing RTL8139 default. Intel support is limited to the listed
legacy devices, not all cards sold under the Intel/E1000 name.

## Validation

```sh
make test-network-host
make test-network-driver
make test-e1000-host
make test-network
python3 tools/test_network.py --nic e1000
python3 tools/test_network_fixture.py --nic e1000 --gui
```

The host tests use AddressSanitizer/UndefinedBehaviorSanitizer to exercise
checksums, DHCP matching, malformed/truncated frames, DNS compression, lease
expiry, the RTL8139 ring boundary, DMA-in-progress, overflow and link state.
QEMU tests exercise actual PCI I/O and DMA, DHCP, repeated gateway pings across
ring wraparound, link loss/recovery and the missing-card case. An independent
Python peer on loopback supplies non-default DHCP configuration and DNS replies,
checks outgoing checksums, and tests hostname ping, routing, inbound ARP/ICMP,
DHCP timeout/cancellation/recovery, and operation alongside the desktop.
These tests require native Linux QEMU, like the other automated tests.

For an additional public-network check:

```sh
python3 tools/test_network.py --internet
```

Public DNS could not be verified in the cloud development machine: its own
resolver also returns a temporary name-resolution failure. Outgoing guest DNS
queries were captured, and the complete DNS path passed against the independent
local peer. A normal WSL/Windows host with working DNS should be tested locally.
