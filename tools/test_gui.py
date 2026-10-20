#!/usr/bin/env python3
"""Exercise real QMP PS/2 mouse events and visible graphical-shell output."""
import argparse
from pathlib import Path
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
    symbols={line.split()[2]:int(line.split()[0],16) for line in subprocess.check_output(['nm','kernel.bin'],text=True).splitlines() if len(line.split())==3}
    with tempfile.TemporaryDirectory(prefix='ironos-gui-',dir='/tmp') as tmp:
        work=Path(tmp)
        with (work/'target.img').open('wb') as f: f.truncate(128*1024*1024)
        subprocess.run(['python3','mkdisk.py',str(work/'data.img'),'disk'],check=True)
        vm=VM(args,work)
        def physical(address,n,name):
            p=work/name;vm.hmp(f'pmemsave {address} {n} "{p}"');return p.read_bytes()
        def frame(): return physical(0xA0000,64000,'frame.bin')
        def visible(x,y,text,timeout=5,color=232):
            # Match the actual pixels against the guest's BIOS font, rather than
            # inferring success from a running process or internal UI variables.
            deadline=time.monotonic()+timeout
            while time.monotonic()<deadline:
                pixels=frame();ok=True
                for i,c in enumerate(text.encode()):
                    for row in range(8):
                        bits=font[c*32+row*2] | font[c*32+row*2+1]
                        for col in range(8):
                            if bool(pixels[(y+row)*320+x+i*8+col]==color) != bool(bits&(128>>col)):
                                ok=False;break
                        if not ok: break
                    if not ok: break
                if ok:return
                time.sleep(.1)
            raise AssertionError(f'Expected visible GUI text {text!r} at {x},{y}')
        px,py=160,100
        def move(x,y):
            nonlocal px,py
            vm.qmp('input-send-event',{'events':[
                {'type':'rel','data':{'axis':'x','value':x-px}},
                {'type':'rel','data':{'axis':'y','value':y-py}}]})
            px,py=x,y;time.sleep(.15)
        def click(x,y):
            move(x,y)
            vm.qmp('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':True}}]})
            time.sleep(.1)
            vm.qmp('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':False}}]})
            time.sleep(.2)
        try:
            vm.expect('kernel:/$>')
            vm.send('desktop');time.sleep(.5)
            font=physical(symbols['saved_font'],65536,'font.bin')
            visible(8,4,'IronOS');visible(168,4,'PS/2 mouse',color=231)
            click(35,65);visible(100,71,'hello.txt')
            click(140,75);click(250,172)
            visible(84,51,'Hello from the IronOS disk!')
            click(100,172);visible(100,71,'hello.txt')
            move(140,75)
            for _ in range(2):
                vm.qmp('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':True}}]})
                time.sleep(.06)
                vm.qmp('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':False}}]})
                time.sleep(.08)
            visible(84,51,'Hello from the IronOS disk!')
            click(100,172)
            click(35,95);visible(84,51,'Memory total')
            if args.screenshot: vm.hmp(f'screendump "{args.screenshot}"')
            # Edge motion clips the cursor without overwriting framebuffer memory.
            move(319,199);move(35,125)
            click(35,125);time.sleep(.3)
            before=frame();move(85,125);time.sleep(.3)
            assert frame()!=before,'Mouse motion did not rotate the 3D camera'
            vm.hmp('sendkey esc');time.sleep(.3)
            visible(8,4,'IronOS')
            click(35,65);click(140,65);click(250,172)
            visible(100,51,'notes.txt')
            click(35,155);vm.expect('kernel:/$>')
            vm.send('cat hello.txt');vm.expect('Hello from the IronOS disk!')
            vm.send('memtest');vm.expect('memtest: PASSED')
            vm.send('desktop');time.sleep(.3)
            vm.hmp('sendkey f');time.sleep(.2);visible(100,71,'hello.txt')
            vm.hmp('sendkey esc');time.sleep(.2);vm.expect('kernel:/$>')
        finally:vm.close()
    print('PASS: PS/2 mouse, clickable file preview/system/launcher, cursor clipping,')
    print('      mouse-look, graphics restoration, keyboard fallback and memory test')


if __name__=='__main__':main()
