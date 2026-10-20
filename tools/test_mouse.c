/* Mock the 8042 controller; exercise actual keyboard routing and mouse decoder. */
#include <stdio.h>
#include <stdlib.h>
#include "../mouse.c"
#include "../keyboard.c"
static struct {unsigned char byte,status;} input[256];
static unsigned int input_head,input_tail;
static unsigned char config=0x41,master_mask=0xFC,slave_mask=0xFF;
static int config_write,aux_write,device_present=1;
static irq_handler_t installed[48];
static void check(int ok,const char *what) { if(!ok){fprintf(stderr,"FAIL: %s\n",what);exit(1);} }
static void feed(int byte,int aux) {
    check(input_head<256,"mock queue bounded");
    input[input_head].byte=byte;input[input_head++].status=1|(aux?0x20:0);
}
unsigned char inb(unsigned short p) {
    if(p==0x64)return input_tail<input_head?input[input_tail].status:0;
    if(p==0x60)return input_tail<input_head?input[input_tail++].byte:0;
    if(p==0x21)return master_mask;
    if(p==0xA1)return slave_mask;
    return 0;
}
void outb(unsigned short p,unsigned char b) {
    if(p==0x21){master_mask=b;return;}
    if(p==0xA1){slave_mask=b;return;}
    if(p==0x64) {
        if(b==0xAD)config|=0x10;
        if(b==0xAE)config&=~0x10;
        if(b==0xA7)config|=0x20;
        if(b==0xA8)config&=~0x20;
        if(b==0x20)feed(config,0);
        if(b==0x60)config_write=1;
        if(b==0xD4)aux_write=1;
    }
    if(p==0x60) {
        if(config_write){config=b;config_write=0;}
        else if(aux_write) {
            aux_write=0;
            if(device_present) {
                feed(0xFA,1);
                if(b==0xFF){feed(0xAA,1);feed(0,1);}
            }
        }
    }
}
void register_interrupt_handler(int n,irq_handler_t h) { check(n<48,"IRQ vector bounded");installed[n]=h; }
static void packet_in(int flags,int dx,int dy) { mouse_receive_byte(flags);mouse_receive_byte(dx);mouse_receive_byte(dy); }
int main(void) {
    keyboard_init();mouse_init();
    check(mouse_present()&&installed[33]==ps2_irq&&installed[44]==ps2_irq,"shared keyboard/mouse handler");
    check((config&0x43)==0x43&&!(config&0x30),"keyboard config preserved; both ports enabled");
    check(master_mask==0xF8&&slave_mask==0xEF,"only cascade and IRQ12 unmasked");
    struct mouse_event e;
    mouse_receive_byte(0);packet_in(0x08,10,5);
    check(mouse_get_event(&e)&&e.x==170&&e.y==95&&e.dx==10&&e.dy==-5,"sync, movement and inverted Y");
    packet_in(0x09,0,0);check(mouse_get_event(&e)&&e.pressed==1,"left press edge");
    packet_in(0x09,0,0);check(mouse_get_event(&e)&&!e.pressed,"held button is not a second click");
    packet_in(0x08,0,0);check(mouse_get_event(&e)&&!e.buttons,"release");
    packet_in(0x18,250,0);check(mouse_get_event(&e)&&e.dx==-6,"nine-bit negative X");
    packet_in(0x08,128,0);check(mouse_get_event(&e)&&e.dx==128,"positive 128 is not sign-extended as eight bits");
    packet_in(0x48,255,255);check(mouse_get_event(&e)&&!e.dx&&!e.dy,"overflow discards motion");
    packet_in(0x08,255,255);check(mouse_get_event(&e)&&e.x==319&&e.y==0,"cursor clips at right/top");
    packet_in(0x38,1,1);check(mouse_get_event(&e)&&e.x==64&&e.y==199,"cursor clips at bottom");
    for(int i=0;i<100;i++)packet_in(0x08,0,0);
    int n=0;while(mouse_get_event(&e))n++;
    check(n==MOUSE_QUEUE-1,"full event queue remains bounded");
    feed(0xE0,0);feed(8,1);feed(1,1);feed(0,1);feed(0x48,0);
    feed(0x11,0);feed(0x91,0);ps2_irq(0);
    check(keyboard_getchar()==KEY_UP&&keyboard_getchar()=='w'&&keyboard_getchar()==-1,"interleaved mouse bytes never become keystrokes");
    check(!keyboard_key_down('w'),"key release preserved");
    check(mouse_get_event(&e)&&e.dx==1,"interleaved mouse packet delivered");
    // Simulate a missing auxiliary device and verify bounded failure restores keyboard.
    present=0;device_present=0;head=tail=cycle=0;input_head=input_tail=0;
    config=0x41;master_mask=0xFC;slave_mask=0xFF;
    mouse_init();check(!mouse_present()&&!(config&0x10)&&(config&1),"no-mouse fallback restores keyboard");
    check(master_mask==0xFC&&slave_mask==0xFF,"failed initialization leaves PIC masks untouched");
    feed(0x1E,0);ps2_irq(0);check(keyboard_getchar()=='a',"keyboard still works after mouse failure");
    puts("PASS: PS/2 initialization/fallback, IRQ routing, sync, signed motion, button edges,");
    puts("      cursor clipping, overflow and bounded event queue");
    return 0;
}
