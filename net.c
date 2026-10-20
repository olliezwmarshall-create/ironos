#include "kernel.h"

/* Small IPv4 stack: Ethernet/ARP, DHCP, UDP DNS and ICMP. No TCP or sockets.
 * All lengths/checksums are checked before accessing received protocol fields.
 */
static unsigned int address, mask, gateway, dns, lease_end;
static unsigned int packets_rx, packets_tx, packets_drop;
static unsigned char incoming[1536], outgoing[1536];
static const unsigned char broadcast[6] = {255,255,255,255,255,255};
static unsigned int neighbor, neighbor_until, arp_wanted;
static unsigned char neighbor_mac[6];
static unsigned short ip_serial, ping_serial, dns_serial;
static unsigned int ping_target, ping_started;
static int ping_waiting, ping_replied;
static unsigned int dns_target, dns_answer;
static unsigned short dns_port;
static unsigned char dns_question[260];
static unsigned int dns_question_len;
static int dns_waiting, dns_error;
struct offer { unsigned int ip, mask, router, dns, server, lease; };
static struct offer offer;
static unsigned int dhcp_xid;
static int dhcp_state; /* 1 DISCOVER, 2 OFFER received/REQUEST, 3 ACK, -1 NAK */

static unsigned short be16(const unsigned char *p) { return (p[0] << 8) | p[1]; }
static unsigned int be32(const unsigned char *p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | (p[2] << 8) | p[3];
}
static void put16(unsigned char *p, unsigned int v) { p[0] = v >> 8; p[1] = v; }
static void put32(unsigned char *p, unsigned int v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static unsigned int sum_words(const unsigned char *p, unsigned int n, unsigned int sum) {
    while (n >= 2) { sum += be16(p); p += 2; n -= 2; }
    if (n) sum += (unsigned int)*p << 8;
    return sum;
}
static unsigned short finish_sum(unsigned int n) {
    while (n >> 16) n = (n & 65535) + (n >> 16);
    return (unsigned short)~n;
}
static unsigned short checksum(const unsigned char *p, unsigned int n) { return finish_sum(sum_words(p, n, 0)); }
static int expired(unsigned int until) { return (int)(uptime_ticks - until) >= 0; }
static void say(const char *s) { print_string(s); newline(); }
static void number(unsigned int n) { char b[16]; int_to_string(n, b); print_string(b); }
static void print_ip(unsigned int ip) {
    for (int i = 24; i >= 0; i -= 8) { number((ip >> i) & 255); if (i) print_string("."); }
}
static int unicast(unsigned int ip) { return (ip >> 24) > 0 && (ip >> 24) < 224 && (ip >> 24) != 127; }
static int parse_ip(const char *s, unsigned int *ip) {
    unsigned int result = 0;
    for (int part = 0; part < 4; part++) {
        unsigned int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + *s++ - '0';
            if (++digits > 3 || v > 255) return 0;
        }
        if (!digits) return 0;
        result = (result << 8) | v;
        if (part < 3) { if (*s++ != '.') return 0; }
    }
    if (*s) return 0;
    *ip = result; return 1;
}
static int valid_offer(const struct offer *o) {
    unsigned int inverse = ~o->mask;
    if (!o->lease || !unicast(o->ip) || !o->mask || (inverse & (inverse + 1)) || !inverse) return 0;
    unsigned int host = o->ip & inverse;
    if (!host || host == inverse) return 0;
    if (o->router && (!unicast(o->router) || ((o->router ^ o->ip) & o->mask))) return 0;
    return 1;
}
static int send_frame(unsigned int n) {
    if (rtl8139_send(outgoing, n)) { packets_drop++; return -1; }
    packets_tx++; return 0;
}
static void ethernet(const unsigned char *to, unsigned int type) {
    memcpy(outgoing, to, 6); memcpy(outgoing + 6, rtl8139_mac(), 6); put16(outgoing + 12, type);
}
static unsigned char *ipv4(const unsigned char *to_mac, unsigned int from, unsigned int to,
                            unsigned int protocol, unsigned int length) {
    ethernet(to_mac, 0x800);
    unsigned char *p = outgoing + 14;
    memset(p, 0, 20); p[0] = 0x45; put16(p + 2, length + 20);
    put16(p + 4, ++ip_serial); put16(p + 6, 0x4000);
    p[8] = 64; p[9] = protocol; put32(p + 12, from); put32(p + 16, to);
    put16(p + 10, checksum(p, 20));
    return p + 20;
}
static int send_udp(const unsigned char *to_mac, unsigned int from, unsigned int to,
                    unsigned short source, unsigned short dest, const unsigned char *data, unsigned int n) {
    if (n > 1472) return -1;
    unsigned char *p = ipv4(to_mac, from, to, 17, n + 8);
    put16(p, source); put16(p + 2, dest); put16(p + 4, n + 8); put16(p + 6, 0);
    memcpy(p + 8, data, n);
    unsigned int sum = sum_words(outgoing + 26, 8, 17 + n + 8);
    unsigned short c = finish_sum(sum_words(p, n + 8, sum));
    put16(p + 6, c ? c : 65535);
    return send_frame(14 + 20 + 8 + n);
}
static void arp_send(const unsigned char *to_mac, unsigned int to_ip, int reply) {
    ethernet(to_mac, 0x806);
    unsigned char *p = outgoing + 14;
    put16(p, 1); put16(p + 2, 0x800); p[4] = 6; p[5] = 4; put16(p + 6, reply ? 2 : 1);
    memcpy(p + 8, rtl8139_mac(), 6); put32(p + 14, address);
    if (reply) memcpy(p + 18, to_mac, 6); else memset(p + 18, 0, 6);
    put32(p + 24, to_ip); send_frame(42);
}
static void receive_arp(const unsigned char *p, unsigned int n, const unsigned char *from_mac) {
    if (n < 28 || be16(p) != 1 || be16(p + 2) != 0x800 || p[4] != 6 || p[5] != 4 ||
        memcmp(p + 8, from_mac, 6) || !address || be32(p + 24) != address) return;
    unsigned int op = be16(p + 6), sender = be32(p + 14);
    if ((op != 1 && op != 2) || !unicast(sender)) return;
    if (sender == arp_wanted) {
        neighbor = sender; memcpy(neighbor_mac, p + 8, 6); neighbor_until = uptime_ticks + 120 * TIMER_HZ;
    }
    if (op == 1) arp_send(p + 8, sender, 1);
}

static void receive_dhcp(const unsigned char *p, unsigned int n) {
    if ((dhcp_state != 1 && dhcp_state != 2) || n < 240 || p[0] != 2 || p[1] != 1 || p[2] != 6 ||
        be32(p + 4) != dhcp_xid || memcmp(p + 28, rtl8139_mac(), 6) || be32(p + 236) != 0x63825363) return;
    struct offer o = dhcp_state == 2 ? offer : (struct offer){0};
    o.ip = be32(p + 16);
    unsigned int message = 0, server = 0, seen = 0;
    for (unsigned int i = 240; i < n;) {
        unsigned int type = p[i++];
        if (type == 255) break;
        if (!type) continue;
        if (i == n) return;
        unsigned int len = p[i++];
        if (len > n - i) return;
        unsigned int bit = type == 53 ? 1 : type == 54 ? 2 : type == 1 ? 4 : type == 3 ? 8 : type == 6 ? 16 : type == 51 ? 32 : 0;
        if (bit && (seen & bit)) return;
        seen |= bit;
        if (type == 53) { if (len != 1) return; message = p[i]; }
        if (type == 54) { if (len != 4) return; server = be32(p + i); }
        if (type == 1) { if (len != 4) return; o.mask = be32(p + i); }
        if (type == 3 || type == 6) {
            if (len < 4 || (len & 3)) return;
            if (type == 3) o.router = be32(p + i); else o.dns = be32(p + i);
        }
        if (type == 51) { if (len != 4) return; o.lease = be32(p + i); }
        i += len;
    }
    if (!unicast(server)) return;
    if (dhcp_state == 1 && message == 2) {
        if (!valid_offer(&o)) return;
        o.server = server; offer = o; dhcp_state = 2;
    } else if (dhcp_state == 2 && server == offer.server) {
        if (message == 6) { dhcp_state = -1; return; }
        if (message != 5 || !(seen & 32) || o.ip != offer.ip || !valid_offer(&o)) return;
        offer = o; dhcp_state = 3;
    }
}
/* Skip an encoded DNS name without following pointers or trusting their targets. */
static int dns_skip_name(const unsigned char *p, unsigned int n, unsigned int *at) {
    unsigned int labels = 0;
    while (*at < n && labels++ < 128) {
        unsigned int len = p[(*at)++];
        if (!len) return 1;
        if ((len & 0xC0) == 0xC0) {
            if (*at >= n) return 0;
            unsigned int target = ((len & 63) << 8) | p[(*at)++];
            return target < n;
        }
        if ((len & 0xC0) || len > n - *at) return 0;
        *at += len;
    }
    return 0;
}
static void receive_dns(const unsigned char *p, unsigned int n) {
    if (!dns_waiting || n < 12 || be16(p) != dns_serial || (be16(p + 2) & 0xF800) != 0x8000) return;
    if (be16(p + 2) & 0x020F) { dns_error = 1; dns_waiting = 0; return; }
    if (be16(p + 4) != 1 || n < 12 + dns_question_len || memcmp(p + 12, dns_question, dns_question_len)) return;
    unsigned int at = 12 + dns_question_len;
    unsigned int records = be16(p + 6);
    if (records > 64) return;
    for (unsigned int i = 0; i < records; i++) {
        if (!dns_skip_name(p, n, &at) || n - at < 10) return;
        unsigned int type = be16(p + at), cls = be16(p + at + 2), len = be16(p + at + 8);
        at += 10;
        if (len > n - at) return;
        if (type == 1 && cls == 1 && len == 4 && unicast(be32(p + at))) {
            dns_answer = be32(p + at); dns_waiting = 0; return;
        }
        at += len;
    }
    dns_error = 1; dns_waiting = 0;
}
static void receive_ipv4(const unsigned char *p, unsigned int n, const unsigned char *from_mac) {
    if (n < 20 || (p[0] >> 4) != 4) return;
    unsigned int h = (p[0] & 15) * 4, total = be16(p + 2);
    if (h < 20 || h > n || total < h || total > n || checksum(p, h) || (be16(p + 6) & 0x3FFF)) return;
    unsigned int source = be32(p + 12), dest = be32(p + 16), protocol = p[9];
    const unsigned char *data = p + h;
    unsigned int len = total - h;
    if (protocol == 17) {
        if (len < 8 || be16(data + 4) < 8 || be16(data + 4) > len) return;
        unsigned int udp_len = be16(data + 4);
        if (be16(data + 6) && finish_sum(sum_words(data, udp_len, sum_words(p + 12, 8, 17 + udp_len)))) return;
        if (be16(data) == 67 && be16(data + 2) == 68 && dhcp_state) receive_dhcp(data + 8, udp_len - 8);
        else if (address && dest == address && source == dns_target && be16(data) == 53 && be16(data + 2) == dns_port)
            receive_dns(data + 8, udp_len - 8);
    } else if (protocol == 1 && address && dest == address && len >= 8 && len <= 1480 && !checksum(data, len)) {
        if (data[0] == 0 && data[1] == 0 && ping_waiting && source == ping_target && len == 16 &&
            be16(data + 4) == 0x494F && be16(data + 6) == ping_serial &&
            be32(data + 8) == ping_started && !memcmp(data + 12, "IRON", 4)) {
            ping_replied = 1; ping_waiting = 0;
        } else if (data[0] == 8 && !data[1] && unicast(source)) {
            unsigned char *reply = ipv4(from_mac, address, source, 1, len);
            memcpy(reply, data, len); reply[0] = 0; put16(reply + 2, 0); put16(reply + 2, checksum(reply, len));
            send_frame(34 + len);
        }
    }
}
static void receive_frame(const unsigned char *p, unsigned int n) {
    if (n < 14 || (memcmp(p, rtl8139_mac(), 6) && memcmp(p, broadcast, 6))) { packets_drop++; return; }
    unsigned int type = be16(p + 12);
    if (type == 0x806) receive_arp(p + 14, n - 14, p + 6);
    if (type == 0x800) receive_ipv4(p + 14, n - 14, p + 6);
}
void net_poll(void) {
    if (!rtl8139_present()) return;
    if (address && expired(lease_end)) { address = mask = gateway = dns = neighbor = 0; }
    for (int budget = 0; budget < 32; budget++) {
        int n = rtl8139_receive(incoming, sizeof(incoming));
        if (!n) break;
        if (n < 0) { packets_drop++; break; }
        packets_rx++; receive_frame(incoming, n);
    }
}
static int ready(void) {
    net_poll();
    if (!rtl8139_present()) { say("No supported Ethernet card. Currently supported: PCI RTL8139 (10EC:8139)."); return 0; }
    if (!rtl8139_link()) { say("Ethernet link down. Check the cable or QEMU network setting."); return 0; }
    return 1;
}
static int configured(void) {
    if (!ready()) return 0;
    if (!address) { say("No IPv4 lease. Run dhcp first."); return 0; }
    return 1;
}
static int resolve_mac(unsigned int target, unsigned char *result) {
    unsigned int hop = ((target ^ address) & mask) ? gateway : target;
    if (!unicast(hop)) { say("No route to that address."); return 0; }
    if (neighbor == hop && !expired(neighbor_until)) { memcpy(result, neighbor_mac, 6); return 1; }
    arp_wanted = hop;
    unsigned int start = uptime_ticks, sent = start - TIMER_HZ;
    while (uptime_ticks - start < 3 * TIMER_HZ) {
        if (!rtl8139_link() || !address) break;
        if (uptime_ticks - sent >= TIMER_HZ) { arp_send(broadcast, hop, 0); sent = uptime_ticks; }
        net_poll();
        if (neighbor == hop && !expired(neighbor_until)) { memcpy(result, neighbor_mac, 6); arp_wanted = 0; return 1; }
        if (keyboard_getchar() == 27) break;
        halt();
    }
    arp_wanted = 0; say("ARP lookup failed (timeout or cancelled)."); return 0;
}
static void send_dhcp(int request) {
    unsigned char p[320]; memset(p, 0, sizeof(p));
    p[0] = 1; p[1] = 1; p[2] = 6; put32(p + 4, dhcp_xid); put16(p + 10, 0x8000);
    memcpy(p + 28, rtl8139_mac(), 6); put32(p + 236, 0x63825363);
    unsigned int at = 240;
    p[at++] = 53; p[at++] = 1; p[at++] = request ? 3 : 1;
    p[at++] = 61; p[at++] = 7; p[at++] = 1; memcpy(p + at, rtl8139_mac(), 6); at += 6;
    p[at++] = 55; p[at++] = 4; p[at++] = 1; p[at++] = 3; p[at++] = 6; p[at++] = 51;
    if (request) {
        p[at++] = 50; p[at++] = 4; put32(p + at, offer.ip); at += 4;
        p[at++] = 54; p[at++] = 4; put32(p + at, offer.server); at += 4;
    }
    p[at] = 255;
    send_udp(broadcast, 0, 0xFFFFFFFF, 68, 67, p, 300);
}
static void dhcp(void) {
    if (!ready()) return;
    address = mask = gateway = dns = neighbor = 0;
    memset(&offer, 0, sizeof(offer));
    dhcp_xid += 0x49524F4E ^ uptime_ticks;
    dhcp_state = 1;
    say("Requesting an IPv4 lease... (Esc cancels)");
    unsigned int start = uptime_ticks, sent = start - 2 * TIMER_HZ;
    int requested = 0;
    while (uptime_ticks - start < 12 * TIMER_HZ && rtl8139_link()) {
        net_poll();
        if (dhcp_state == 3 || dhcp_state == -1) break;
        if (uptime_ticks - sent >= 2 * TIMER_HZ || (dhcp_state == 2 && !requested)) {
            requested = dhcp_state == 2; send_dhcp(requested); sent = uptime_ticks;
        }
        if (keyboard_getchar() == 27) break;
        halt();
    }
    int success = dhcp_state == 3;
    dhcp_state = 0;
    if (!success) { say("DHCP failed (timeout, declined, link down or cancelled)."); return; }
    address = offer.ip; mask = offer.mask; gateway = offer.router; dns = unicast(offer.dns) ? offer.dns : 0;
    unsigned int seconds = offer.lease ? offer.lease : 3600;
    if (seconds > 86400) seconds = 86400;
    lease_end = uptime_ticks + seconds * TIMER_HZ;
    print_string("DHCP lease acquired: "); print_ip(address); newline();
}
static unsigned int lookup(const char *name) {
    if (!configured()) return 0;
    if (!dns) { say("No DNS server supplied by DHCP."); return 0; }
    unsigned int length = strlen(name);
    if (!length || length > 253) { say("Invalid host name."); return 0; }
    dns_question_len = 0;
    for (unsigned int at = 0; at < length;) {
        unsigned int label = dns_question_len++, n = 0;
        while (at < length && name[at] != '.') {
            char c = name[at++];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') || ++n > 63) {
                say("Invalid host name."); return 0;
            }
            dns_question[dns_question_len++] = c;
        }
        if (!n) { say("Invalid host name."); return 0; }
        dns_question[label] = n;
        if (at < length) at++;
    }
    dns_question[dns_question_len++] = 0;
    put16(dns_question + dns_question_len, 1); put16(dns_question + dns_question_len + 2, 1); dns_question_len += 4;
    unsigned char mac[6];
    if (!resolve_mac(dns, mac)) return 0;
    unsigned char query[280]; memset(query, 0, 12);
    dns_serial++; put16(query, dns_serial); put16(query + 2, 0x100); put16(query + 4, 1);
    memcpy(query + 12, dns_question, dns_question_len);
    dns_port = 49152 + (dns_serial & 0x3FFF); dns_target = dns;
    dns_answer = 0; dns_error = 0; dns_waiting = 1;
    unsigned int start = uptime_ticks, sent = start - 2 * TIMER_HZ;
    while (uptime_ticks - start < 6 * TIMER_HZ && dns_waiting && address && rtl8139_link()) {
        if (uptime_ticks - sent >= 2 * TIMER_HZ) {
            send_udp(mac, address, dns, dns_port, 53, query, 12 + dns_question_len); sent = uptime_ticks;
        }
        net_poll();
        if (keyboard_getchar() == 27) break;
        halt();
    }
    dns_waiting = 0;
    if (!dns_answer) say(dns_error ? "DNS returned an error or no usable IPv4 answer." : "DNS timed out or was cancelled.");
    return dns_answer;
}
static void ping(const char *arg) {
    if (!configured()) return;
    unsigned int target;
    if (!parse_ip(arg, &target)) { target = lookup(arg); if (!target) return; }
    if (!unicast(target) || target == address ||
        (!((target ^ address) & mask) && ((target & ~mask) == 0 || (target & ~mask) == ~mask))) { say("Use a unicast remote IPv4 address (loopback is not implemented)."); return; }
    unsigned char mac[6];
    if (!resolve_mac(target, mac)) return;
    unsigned int replies = 0;
    print_string("Pinging "); print_ip(target); say(" (3 probes, Esc cancels)");
    for (int i = 0; i < 3 && address && rtl8139_link(); i++) {
        unsigned char *p = ipv4(mac, address, target, 1, 16);
        memset(p, 0, 16); p[0] = 8; put16(p + 4, 0x494F); put16(p + 6, ++ping_serial);
        ping_started = uptime_ticks; put32(p + 8, ping_started); memcpy(p + 12, "IRON", 4);
        put16(p + 2, checksum(p, 16));
        ping_target = target; ping_waiting = 1; ping_replied = 0;
        if (send_frame(50)) { ping_waiting = 0; say("Transmit failed."); break; }
        int cancelled = 0;
        while (!ping_replied && uptime_ticks - ping_started < TIMER_HZ && address && rtl8139_link()) {
            net_poll();
            if (keyboard_getchar() == 27) { cancelled = 1; break; }
            halt();
        }
        ping_waiting = 0;
        if (cancelled) { say("Cancelled."); break; }
        if (ping_replied) {
            replies++; print_string("Reply from "); print_ip(target); print_string(" time=");
            number((uptime_ticks - ping_started) * (1000 / TIMER_HZ)); say(" ms");
        } else say("Request timed out (ICMP may be blocked).");
    }
    number(replies); say("/3 replies");
}
static void info(void) {
    net_poll();
    if (!rtl8139_present()) { ready(); return; }
    say("Ethernet: RTL8139 (PCI 10EC:8139), polled driver");
    print_string("MAC: ");
    const unsigned char *m = rtl8139_mac();
    for (int i = 0; i < 6; i++) {
        char h[9]; hex_to_string(m[i], h); print_string(h + 6); if (i < 5) print_string(":");
    }
    newline(); say(rtl8139_link() ? "Link: up" : "Link: down");
    if (address) {
        print_string("IPv4: "); print_ip(address); print_string("  Mask: "); print_ip(mask); newline();
        print_string("Gateway: "); print_ip(gateway); print_string("  DNS: "); print_ip(dns); newline();
        print_string("Lease: "); number((lease_end - uptime_ticks) / TIMER_HZ); say(" seconds left; run dhcp to renew.");
    } else say("IPv4: not configured (run dhcp).");
    print_string("Packets RX/TX/drop: "); number(packets_rx); print_string("/"); number(packets_tx); print_string("/"); number(packets_drop); newline();
}
void net_command(const char *cmd, const char *arg) {
    if (!strcmp(cmd, "net") && !*arg) info();
    else if (!strcmp(cmd, "dhcp") && !*arg) dhcp();
    else if (!strcmp(cmd, "ping") && *arg) ping(arg);
    else if (!strcmp(cmd, "resolve") && *arg) {
        unsigned int ip = lookup(arg);
        if (ip) { print_string(arg); print_string(" = "); print_ip(ip); newline(); }
    } else say("Usage: net | dhcp | ping <IPv4-or-host> | resolve <host>");
}
