#include "kernel.h"

/* A single-window graphical shell. Applications are still cooperative kernel code. */
#define PAGE_ROWS 10
#define FILE_LIMIT 128
static struct fat_dirent files[FILE_LIMIT];
static unsigned char preview[8192];
static int view, count, selected, previewing, preview_len, preview_page, truncated;
static int pointer_x, pointer_y, last_click = -1;
static unsigned long last_click_time;
static const char *notice;
static const char *apps[] = {"Home", "Files", "System", "Demo3D", "Shell"};

static int inside(int x,int y,int left,int top,int w,int h) {
    return x>=left && x<left+w && y>=top && y<top+h;
}
static void text(int x,int y,const char *s,int max) { graphics_text(x,y,s,232,max); }
static void button(int x,int y,int w,int h,const char *name,int active) {
    graphics_rect(x,y,w,h,230);
    graphics_rect(x+1,y+1,w-2,h-2,active || inside(pointer_x,pointer_y,x,y,w,h) ? 228 : 227);
    text(x+(w<20?2:4),y+(h-8)/2,name,w<20?1:(w-8)/8);
}
static void collect(const struct fat_dirent *e,void *ctx) {
    (void)ctx;
    if (!strcmp(e->name,".") || !strcmp(e->name,"..")) return;
    if (count==FILE_LIMIT) { truncated=1; return; }
    files[count++]=*e;
}
static void refresh(void) {
    count=selected=previewing=truncated=0; last_click=-1; notice=0;
    if (!fat_mounted()) { notice="Use mount in the terminal."; return; }
    struct fat_dirent dir;
    int r=fat_lookup(".",&dir);
    if (!r) r=fat_list_dir(&dir,collect,0);
    if (r) notice=fat_strerror(r);
    else if (truncated) notice="First 128 entries shown.";
}
static void open_file(void) {
    if (!count) return;
    struct fat_dirent *e=&files[selected];
    if (e->attr & FAT_ATTR_DIR) {
        int r=fat_chdir(e->name);
        if (r) notice=fat_strerror(r); else refresh();
    } else {
        preview_len=fat_read_file(e,preview,sizeof(preview));
        if (preview_len<0) { notice="Cannot read this file."; return; }
        previewing=1; preview_page=0;
        notice=e->size>sizeof(preview) ? "Preview: first 8 KiB only." : 0;
    }
}
static int preview_rows(void) {
    int row=0,col=0;
    for (int i=0;i<preview_len;i++) {
        if (preview[i]=='\r') continue;
        if (preview[i]=='\n') { row++;col=0; }
        else if (++col==27) { col=0;row++; }
    }
    return row+(col!=0);
}
static void parent(void) {
    if (previewing) { previewing=0;notice=0;return; }
    if (fat_mounted()) {
        if (fat_chdir("..")) notice="Cannot open parent.";else refresh();
    }
}
static void page(int direction) {
    if (previewing) {
        int max=(preview_rows()-1)/PAGE_ROWS;
        if (direction>0 && preview_page<max) preview_page++;
        if (direction<0 && preview_page) preview_page--;
    } else if (count) {
        selected+=direction*PAGE_ROWS;
        if (selected<0) selected=0;
        if (selected>=count) selected=count-1;
    }
}
static void draw_preview(void) {
    int row=0,col=0,first=preview_page*PAGE_ROWS;
    for (int i=0;i<preview_len && row<first+PAGE_ROWS;i++) {
        unsigned char c=preview[i];
        if (c=='\r') continue;
        if (c=='\n') { row++;col=0;continue; }
        if (row>=first) {
            char s[2]={c=='\t'?' ':c>=32 && c<127?c:'.',0};
            text(84+col*8,51+(row-first)*10,s,1);
        }
        if (++col==27) { row++;col=0; }
    }
}
static void metric(int row,const char *name,unsigned int value,const char *unit) {
    char n[16];int_to_string(value,n);
    text(84,row,name,26); text(84,row+10,n,12);text(84+strlen(n)*8,row+10,unit,12);
}
static void draw(void) {
    graphics_rect(0,0,320,200,224);
    graphics_rect(0,0,320,16,226);
    text(8,4,"IronOS",12);
    graphics_text(168,4,mouse_present()?"PS/2 mouse":"Keyboard mode",231,18);
    for (int i=0;i<5;i++) button(8,24+i*30,60,22,apps[i],i==view);
    graphics_rect(78,26,236,156,234); /* window shadow */
    graphics_rect(76,24,236,156,225);
    graphics_rect(76,24,236,19,226);
    text(84,29,view==1?(previewing?files[selected].name:fat_getcwd()):view==2?"System":"Welcome",26);
    button(296,27,12,12,"X",0);
    if (view==0) {
        text(88,54,"Welcome to IronOS",25);
        graphics_text(88,73,"A small, independent OS.",231,25);
        text(88,89,"Click an app to explore.",25);
        button(88,111,100,24,"3D Demo",0);
        button(196,111,100,24,"Files",0);
        graphics_text(88,148,"H F S G T: shortcuts",231,25);
        graphics_text(88,161,"Esc: return to terminal",231,25);
    } else if (view==2) {
        metric(51,"Memory total",pmm_total_pages()/256," MiB");
        metric(77,"Memory free",pmm_free_page_count()/256," MiB");
        int disks=0;
        for (int i=0;i<disk_count();i++) if (disk_get(i)->usable) disks++;
        metric(103,"Storage disks",disks,"");
        text(84,133,rtl8139_present()?(rtl8139_link()?"Ethernet: link up":"Ethernet: link down"):"Ethernet: no driver",26);
        graphics_text(84,151,"CPU-rendered VGA graphics",231,27);
        graphics_text(84,163,"32-bit x86 | BIOS boot",231,27);
    } else {
        if (previewing) draw_preview();
        else {
            int first=selected/PAGE_ROWS*PAGE_ROWS;
            for (int i=first;i<count && i<first+PAGE_ROWS;i++) {
                int y=50+(i-first)*10;
                if (i==selected) graphics_rect(82,y,222,10,228);
                text(84,y+1,files[i].attr & FAT_ATTR_DIR?">":" ",1);
                text(100,y+1,files[i].name,25);
            }
            if (!count) graphics_text(84,59,fat_mounted()?"This folder is empty.":"No filesystem mounted.",231,27);
        }
        if (notice) graphics_text(84,156,notice,233,27);
        button(84,167,40,11,previewing?"Back":"Up",0);
        button(128,167,48,11,"Prev",0);
        button(180,167,48,11,"Next",0);
        button(232,167,72,11,"Open",0);
    }
    graphics_rect(0,188,320,12,226);
    graphics_text(8,190,view==1?"Double-click/Enter: open | Esc: back":"Click an app | Esc: terminal",232,38);
    if (mouse_present()) {
        for (int y=0;y<11;y++)
            for (int x=0;x<=y/2;x++)
                graphics_rect(pointer_x+x,pointer_y+y,1,1,x==0 || x==y/2 || y==10?234:232);
    }
    graphics_present();
}
/* Return 1 for terminal, 2 for game; other actions stay in the shell. */
static int activate(int app) {
    if (app==4) return 1;
    if (app==3) return 2;
    view=app; notice=0;
    if (view==1) refresh();
    return 0;
}
static int click(int x,int y) {
    for (int i=0;i<5;i++) if (inside(x,y,8,24+i*30,60,22)) return activate(i);
    if (inside(x,y,296,27,12,12)) { if (!view) return 1;view=0;return 0; }
    if (view==0) {
        if (inside(x,y,88,111,100,24)) return 2;
        if (inside(x,y,196,111,100,24)) activate(1);
    } else if (view==1) {
        if (inside(x,y,84,167,40,11)) parent();
        else if (inside(x,y,128,167,48,11)) page(-1);
        else if (inside(x,y,180,167,48,11)) page(1);
        else if (inside(x,y,232,167,72,11) && !previewing) open_file();
        else if (!previewing && inside(x,y,82,50,222,PAGE_ROWS*10)) {
            int index=selected/PAGE_ROWS*PAGE_ROWS+(y-50)/10;
            if (index<count) {
                selected=index;
                if (last_click==index && uptime_ticks-last_click_time<TIMER_HZ/2) { last_click=-1;open_file(); }
                else { last_click=index;last_click_time=uptime_ticks; }
            }
        }
    }
    return 0;
}
void gui_run(void) {
    char saved_dir[256];int restore=fat_mounted();
    if (restore) memcpy(saved_dir,fat_getcwd(),strlen(fat_getcwd())+1);
    struct mouse_event event;
    while (mouse_get_event(&event)) {}
    mouse_get_state(&event);pointer_x=event.x;pointer_y=event.y;
    view=previewing=0;notice=0;
    graphics_begin();draw();
    unsigned long last=uptime_ticks;
    int dirty=0;
    for (;;) {
        net_poll();
        int action=0;
        while (mouse_get_event(&event)) {
            pointer_x=event.x;pointer_y=event.y;dirty=1;
            if (event.pressed&1) action=click(pointer_x,pointer_y);
            if ((event.pressed&2) && view==1) parent();
            if (action) break;
        }
        int key=keyboard_getchar();
        if (key>=0) {
            dirty=1;
            if (key==27 && view==1 && previewing) { previewing=0;notice=0; }
            else if (key==27 || key=='t' || key=='T' || key=='q') action=1;
            else if (key=='h' || key=='H') activate(0);
            else if (key=='f' || key=='F') activate(1);
            else if (key=='s' || key=='S') activate(2);
            else if (key=='g' || key=='G') action=2;
            else if (key=='\t') activate((view+1)%3);
            else if (view==1) {
                if (key=='\b') parent();
                if (key==KEY_PGUP) page(-1);
                if (key==KEY_PGDN) page(1);
                if (previewing && key==KEY_UP) page(-1);
                if (previewing && key==KEY_DOWN) page(1);
                if (!previewing) {
                    if (key=='\n') open_file();
                    if (key==KEY_UP && selected) selected--;
                    if (key==KEY_DOWN && selected+1<count) selected++;
                    if (key==KEY_HOME) selected=0;
                    if (key==KEY_END && count) selected=count-1;
                }
            }
        }
        if (action==1) break;
        if (action==2) {
            graphics_end();graphics_demo();graphics_begin();dirty=1;
            while (mouse_get_event(&event)) {}
            mouse_get_state(&event);pointer_x=event.x;pointer_y=event.y;
        }
        if (view==2 && uptime_ticks/TIMER_HZ!=last/TIMER_HZ) dirty=1;
        if (dirty && uptime_ticks-last>=2) { draw();dirty=0;last=uptime_ticks; }
        halt();
    }
    graphics_end();
    if (restore) fat_chdir(saved_dir);
}
