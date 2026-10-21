/* Packet parsing and checksum regressions, including malformed input. */
#include <stdio.h>
#include <stdlib.h>
#include "../net.c"
volatile unsigned long uptime_ticks;
static unsigned char test_mac[6] = {2,0,0,0,0,1}, sent_packet[1536];
static unsigned int sent_size, sends;
int nic_present(void) { return 1; }
int nic_link(void) { return 1; }
const unsigned char *nic_mac(void) { return test_mac; }
int nic_send(const void *p, unsigned int n) {
    if (n > sizeof(sent_packet)) abort();
    memcpy(sent_packet, p, n); sent_size = n; sends++; return 0;
}
int nic_receive(unsigned char *p, unsigned int n) { (void)p; (void)n; return 0; }
int keyboard_getchar(void) { return -1; }
void print_string(const char *s) { (void)s; }
void newline(void) {}
void int_to_string(unsigned long n, char *p) { sprintf(p, "%lu", n); }
void hex_to_string(unsigned int n, char *p) { sprintf(p, "%08X", n); }
void *memcpy(void *d, const void *s, unsigned int n) {
    unsigned char *a=d; const unsigned char *b=s;
    for (unsigned int i=0;i<n;i++) a[i]=b[i];
    return d;
}
void *memset(void *d, int v, unsigned int n) {
    unsigned char *a=d; for (unsigned int i=0;i<n;i++) a[i]=v; return d;
}
int memcmp(const void *a, const void *b, unsigned int n) {
    const unsigned char *p=a,*q=b;
    for (unsigned int i=0;i<n;i++) if (p[i]!=q[i]) return p[i]-q[i];
    return 0;
}
unsigned int strlen(const char *s) { unsigned int n=0; while(s[n])n++; return n; }
int strcmp(const char *a,const char *b) { while(*a && *a==*b){a++;b++;} return *a-*b; }
static void check(int ok, const char *what) { if(!ok){fprintf(stderr,"FAIL: %s\n",what);exit(1);} }
static void bootp(unsigned char *p, int message) {
    memset(p,0,320); p[0]=2;p[1]=1;p[2]=6;
    put32(p+4,dhcp_xid);put32(p+16,0x0A00020F);memcpy(p+28,test_mac,6);put32(p+236,0x63825363);
    unsigned int at=240;
    p[at++]=53;p[at++]=1;p[at++]=message;
    p[at++]=54;p[at++]=4;put32(p+at,0x0A000202);at+=4;
    p[at++]=1;p[at++]=4;put32(p+at,0xFFFFFF00);at+=4;
    p[at++]=3;p[at++]=4;put32(p+at,0x0A000202);at+=4;
    p[at++]=6;p[at++]=4;put32(p+at,0x0A000203);at+=4;
    p[at++]=51;p[at++]=4;put32(p+at,3600);at+=4;p[at]=255;
}
static unsigned int dns_reply(unsigned char *p) {
    memset(p,0,512); put16(p,42);put16(p+2,0x8180);put16(p+4,1);put16(p+6,1);
    dns_question_len=7; dns_question[0]=1;dns_question[1]='a';dns_question[2]=0;
    put16(dns_question+3,1);put16(dns_question+5,1);
    memcpy(p+12,dns_question,7);
    unsigned int at=19;p[at++]=0xC0;p[at++]=12;put16(p+at,1);at+=2;put16(p+at,1);at+=2;
    put32(p+at,60);at+=4;put16(p+at,4);at+=2;put32(p+at,0xC0000201);at+=4;
    dns_serial=42;dns_answer=dns_error=0;dns_waiting=1;return at;
}
int main(void) {
    unsigned char b[1600], packet[1536]; unsigned int ip;
    const unsigned char vector[]={0,1,0xF2,3,0xF4,0xF5,0xF6,0xF7};
    check(checksum(vector,8)==0x220D,"RFC1071 checksum vector");
    check(checksum(vector,5)==0x19FA,"odd-length checksum");
    check(parse_ip("10.0.2.2",&ip)&&ip==0x0A000202,"IPv4 parsing");
    check(!parse_ip("256.1.2.3",&ip)&&!parse_ip("1.2.3",&ip)&&!parse_ip("1.2.3.4x",&ip),"bad IPv4 rejected");
    dhcp_xid=123;dhcp_state=1;bootp(b,2);
    receive_dhcp(b,241);check(dhcp_state==1,"truncated DHCP rejected");
    b[4]^=1;receive_dhcp(b,300);check(dhcp_state==1,"DHCP transaction matched");
    bootp(b,2);b[28]^=1;receive_dhcp(b,300);check(dhcp_state==1,"DHCP client MAC matched");
    bootp(b,2);put32(b+251,0xFF00FF00);receive_dhcp(b,300);check(dhcp_state==1,"noncontiguous mask rejected");
    bootp(b,2);receive_dhcp(b,300);check(dhcp_state==2&&offer.ip==0x0A00020F,"DHCP offer accepted");
    bootp(b,5);put32(b+245,0x0A000299);receive_dhcp(b,300);check(dhcp_state==2,"foreign DHCP server rejected");
    bootp(b,5);receive_dhcp(b,300);check(dhcp_state==3,"matching DHCP ACK accepted");dhcp_state=0;
    unsigned int len=dns_reply(b);
    receive_dns(b,len-1);check(dns_waiting,"truncated DNS record rejected");
    receive_dns(b,len);check(dns_answer==0xC0000201&&!dns_waiting,"compressed DNS answer accepted");
    len=dns_reply(b);b[20]=255;receive_dns(b,len);check(dns_waiting,"out-of-range DNS pointer rejected");
    len=dns_reply(b);b[2]|=2;receive_dns(b,len);check(dns_error&&!dns_waiting,"DNS truncation reported");
    address=0x0A00020F;mask=0xFFFFFF00;lease_end=10000;dns_target=0x0A000203;dns_port=50000;
    len=dns_reply(b);send_udp(test_mac,dns_target,address,53,dns_port,b,len);
    memcpy(packet,sent_packet,sent_size);unsigned int frame_len=sent_size;
    packet[24]^=1;receive_frame(packet,frame_len);check(!dns_answer,"bad IPv4 checksum rejected");
    memcpy(packet,sent_packet,frame_len);packet[40]^=1;receive_frame(packet,frame_len);check(!dns_answer,"bad UDP checksum rejected");
    memcpy(packet,sent_packet,frame_len);put16(packet+20,0x2000);put16(packet+24,0);put16(packet+24,checksum(packet+14,20));
    receive_frame(packet,frame_len);check(!dns_answer,"fragmented packet rejected");
    receive_frame(sent_packet,frame_len);check(dns_answer==0xC0000201,"IPv4/UDP DNS dispatch and checksums");
    unsigned char *p=ipv4(test_mac,0x0A000202,address,1,16);
    memset(p,0,16);p[0]=8;put16(p+2,checksum(p,16));
    memcpy(packet,outgoing,50);unsigned int before=sends;
    receive_frame(packet,50);check(sends==before+1&&sent_packet[34]==0&&!checksum(sent_packet+34,16),"ICMP echo response");
    // Exercise every truncated prefix and many arbitrary packet lengths under ASan/UBSan.
    for (unsigned int n=0;n<50;n++) receive_frame(packet,n);
    unsigned int random=1;
    for (int run=0;run<20000;run++) {
        unsigned int n=run%1600;
        for(unsigned int i=0;i<n;i++){random=random*1664525u+1013904223u;b[i]=random>>24;}
        if (n >= 14) {
            memcpy(b, test_mac, 6); put16(b + 12, run & 1 ? 0x800 : 0x806);
            if ((run & 1) && n >= 34) {
                b[14] = 0x45; put16(b + 16, n - 14); put16(b + 20, 0);
                b[23] = run & 2 ? 17 : 1; put32(b + 26, dns_target); put32(b + 30, address);
                put16(b + 24, 0); put16(b + 24, checksum(b + 14, 20));
            }
        }
        receive_frame(b,n);
    }
    lease_end=0;uptime_ticks=1;net_poll();check(!address&&!gateway&&!dns,"lease expiry clears network config");
    net_command("netconfig","10.0.2.15 255.255.255.0 10.0.2.2 10.0.2.3");
    check(address==0x0A00020F && manual,"valid static configuration");
    uptime_ticks=100000;net_poll();check(address==0x0A00020F,"static configuration does not expire");
    net_command("netconfig","10.0.2.15 255.0.255.0 10.0.2.2 10.0.2.3");
    check(address==0x0A00020F && mask==0xFFFFFF00,"invalid config preserves current settings");
    net_command("netoff","");check(!address && !manual,"disconnect clears configuration");
    puts("PASS: checksums, DHCP matching/validation, DNS compression, malformed/truncated packets,");
    puts("      IPv4/UDP dispatch, fragmentation rejection, ICMP replies and lease expiry");
    return 0;
}

const char *nic_name(void) { return "Test Ethernet"; }
void nic_list(void) {}
