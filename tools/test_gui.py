#!/usr/bin/env python3
"""Verify framebuffer pixels, window controls, apps and text-mode restoration."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
import time
from test_install import VM


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--qemu',default='qemu-system-x86_64')
    ap.add_argument('--firmware',action='append',default=[])
    ap.add_argument('--screenshot')
    args=ap.parse_args()
    symbols={p[2]:int(p[0],16) for line in subprocess.check_output(['nm','kernel.bin'],text=True).splitlines() if len(p:=line.split())==3}
    with tempfile.TemporaryDirectory(prefix='ironos-windows-',dir='/tmp') as tmp:
        work=Path(tmp)
        with (work/'target.img').open('wb') as f:f.truncate(128*1024*1024)
        subprocess.run(['python3','mkdisk.py',str(work/'data.img'),'disk'],check=True)
        vm=VM(args,work)
        def physical(addr,n,name='memory.bin'):
            p=work/name;vm.hmp(f'pmemsave {addr} {n} "{p}"');return p.read_bytes()
        def key(k): vm.hmp('sendkey '+k);time.sleep(.2)
        def visible(x,y,s,timeout=5):
            deadline=time.monotonic()+timeout
            while time.monotonic()<deadline:
                data=physical(base,800*600*4)
                ok=True
                for i,c in enumerate(s.encode()):
                    for row in range(16):
                        bits=font[c*32+row]
                        for col in range(8):
                            rgb=struct.unpack_from('<I',data,((y+row)*800+x+i*8+col)*4)[0]
                            if bool(rgb==0xfafafa)!=bool(bits&(128>>col)):
                                ok=False;break
                        if not ok:break
                    if not ok:break
                if ok:return
                time.sleep(.1)
            raise AssertionError(f'Missing visible text {s!r} at {x},{y}')
        def move(x,y):
            # Keep PS/2 packets below the device's per-report motion limit.
            for axis,sym,target in [('x','mx',x),('y','my',y)]:
                current=struct.unpack('<i',physical(symbols[sym],4))[0]
                delta=target-current
                while delta:
                    step=max(-100,min(100,delta))
                    vm.qmp('input-send-event',{'events':[{'type':'rel','data':{'axis':axis,'value':step}}]})
                    delta-=step;time.sleep(.06)
            time.sleep(.15)
        def down(value):
            vm.qmp('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':value}}]});time.sleep(.15)
        def click(x,y):move(x,y);down(True);down(False)
        try:
            vm.expect('kernel:/$>')
            vm.send('calc (12 + 3) * 4');vm.expect('60')
            vm.send('calc 1 / 0');vm.expect('division by zero')
            vm.send('desktop');time.sleep(.5)
            base=struct.unpack('<I',physical(symbols['lfb'],4))[0]
            assert base,'800x600 framebuffer was not enabled'
            font=physical(symbols['saved_font'],65536)
            visible(78,78,'Welcome to IronOS')
            key('ctrl-a');visible(158,158,'Integer calculator')
            for k in ['2','shift-8','3','ret']:key(k)
            visible(158,218,'6')
            # Drag Calc using actual PS/2 mouse events.
            move(180,140);down(True)
            vm.qmp('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':90}},{'type':'rel','data':{'axis':'y','value':40}}]})
            time.sleep(.2);down(False)
            visible(248,198,'Integer calculator')
            # Minimize, restore from taskbar, then close and reopen.
            click(680,180);visible(78,78,'Welcome to IronOS')
            click(150,588);visible(248,198,'Integer calculator')
            click(700,180);visible(78,78,'Welcome to IronOS')
            key('ctrl-n');visible(138,138,'No supported Ethernet')
            key('ctrl-c');visible(178,278,'RTC hardware time')
            # Tab visits every visible app, even with overlapping windows.
            for x,y in [(71,51),(131,111),(171,151)]:
                key('tab')
                assert struct.unpack('<I',physical(base+(y*800+x)*4,4))[0]==0x488dba,'Tab did not cycle focus'
            key('ctrl-o')
            for k in ['h','i']:key(k)
            visible(198,198,'hi')
            key('ctrl-s');visible(198,480,'Mount a disk rw to save notes.')
            key('ctrl-f');visible(96,73,'/')
            key('esc');vm.expect('kernel:/$>')
            vm.send('echo TEXT_RESTORED');vm.expect('TEXT_RESTORED')
            vm.send('mount 0 rw');vm.expect('kernel:/#>')
            # Repeated transitions must preserve VGA font and apps.
            vm.send('desktop');time.sleep(.3);visible(78,78,'Welcome to IronOS')
            if args.screenshot:vm.hmp(f'screendump "{args.screenshot}"')
            key('ctrl-g');time.sleep(.3);key('esc');time.sleep(.3)
            visible(78,78,'Welcome to IronOS')
            key('ctrl-o');key('ctrl-s');visible(198,480,'Saved desktop.txt in current folder')
            key('ctrl-s');visible(198,480,'Ctrl+S again replaces desktop.txt')
            key('ctrl-s');visible(198,480,'Saved desktop.txt in current folder')
            key('esc');vm.send('cat desktop.txt');vm.expect('hi')
            vm.send('memtest');vm.expect('memtest: PASSED')
        finally:vm.close()
        subprocess.run(['python3','tools/fsck.py',str(work/'data.img')],check=True)
        args.extra_flags='-vga cirrus'
        vm=VM(args,work)
        try:
            vm.expect('kernel:/$>');vm.send('desktop');time.sleep(.5)
            assert physical(symbols['lfb'],4)==bytes(4),'Unknown GPU did not use VGA fallback'
            font=physical(symbols['saved_font'],65536);pixels=physical(0xa0000,64000)
            for i,c in enumerate(b'Welcome to IronOS'):
                for row in range(8):
                    bits=font[c*32+row*2]|font[c*32+row*2+1]
                    for col in range(8):
                        assert bool(pixels[(38+row)*320+18+i*8+col]==232)==bool(bits&(128>>col))
            key('esc');vm.send('echo FALLBACK_OK');vm.expect('FALLBACK_OK')
        finally:vm.close()
    print('PASS: 800x600 pixels, calculator, mouse drag, minimize/restore/close,')
    print('      app focus, notes saving/overwrite guards, VGA fallback and game/text transitions')

if __name__=='__main__':main()
