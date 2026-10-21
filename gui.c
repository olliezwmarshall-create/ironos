#include "kernel.h"

/* Cooperative window manager: bounded windows, focus, stacking, dragging and
 * minimization. Apps remain kernel code; this does not imply process isolation. */
#define APPS 7
#define LIMIT 128
static const char *names[APPS]={"Home","Files","System","Network","Calc","Clock","Notes"};
struct window { int x,y,w,h,open,minimized; };
static struct window windows[APPS];
static int order[APPS], focus, drag=-1, drag_x,drag_y,mx,my,sw,sh,fh,bar;
static struct fat_dirent files[LIMIT];
static unsigned char preview[8192];
static int count,selected,previewing,preview_len,preview_page,last_click=-1;
static unsigned long last_click_time;
static char expression[96],answer[64],notes[2048];
static int expr_len,notes_len,save_confirm;
static const char *notice;
static int inside(int x,int y,int l,int t,int w,int h) { return x>=l && x<l+w && y>=t && y<t+h; }
static void text(int x,int y,const char *s,int max) { graphics_text(x,y,s,232,max); }
static void button(int x,int y,int w,int h,const char *s,int active) {
    graphics_rect(x,y,w,h,active?228:227);text(x+3,y+(h-fh)/2,s,(w-6)/8);
}
static void line(struct window *w,int row,const char *s) {
    int y=w->y+bar+6+row*(fh+4);
    if (y+fh<=w->y+w->h-4) text(w->x+8,y,s,(w->w-16)/8);
}
static void number_line(struct window *w,int row,const char *s,unsigned int n) {
    char buf[80],num[16];int_to_string(n,num);int at=0;
    for (int i=0;s[i] && at<60;i++) buf[at++]=s[i];
    for (int i=0;num[i] && at<79;i++) buf[at++]=num[i];
    buf[at]=0;line(w,row,buf);
}
static int rows(struct window *w) { int n=(w->h-bar-34)/(fh+4);return n>0?n:1; }
static int columns(struct window *w) { return (w->w-16)/8; }
static void collect(const struct fat_dirent *e,void *ctx) {
    (void)ctx;if (!strcmp(e->name,".") || !strcmp(e->name,"..")) return;
    if (count<LIMIT) files[count++]=*e;
}
static void refresh(void) {
    count=selected=previewing=0;last_click=-1;notice=0;
    struct fat_dirent dir;
    if (!fat_mounted()) { notice="Use mount in the terminal.";return; }
    int r=fat_lookup(".",&dir);if (!r) r=fat_list_dir(&dir,collect,0);
    if (r) notice=fat_strerror(r);
}
static void open_file(void) {
    if (!count) return;
    struct fat_dirent *e=&files[selected];
    if (e->attr&FAT_ATTR_DIR) { int r=fat_chdir(e->name);if (r) notice=fat_strerror(r);else refresh(); }
    else { preview_len=fat_read_file(e,preview,sizeof(preview));
        if (preview_len<0) notice="Cannot read file.";
        else { previewing=1;preview_page=0;notice=e->size>sizeof(preview)?"First 8 KiB only":0; }
    }
}
static void parent(void) {
    if (previewing) { previewing=0;notice=0; }
    else if (fat_mounted()) { if (fat_chdir("..")) notice="Cannot open parent";else refresh(); }
}
static int preview_rows(struct window *w) {
    int row=0,col=0;
    for (int i=0;i<preview_len;i++) {
        if (preview[i]=='\r') continue;
        if (preview[i]=='\n') { row++;col=0; }
        else if (++col==columns(w)) { row++;col=0; }
    }
    return row+(col!=0);
}
static void page(int delta) {
    struct window *w=&windows[1];int r=rows(w);
    if (previewing) {
        int max=(preview_rows(w)-1)/r;
        preview_page+=delta;if (preview_page<0) preview_page=0;if (preview_page>max) preview_page=max<0?0:max;
    } else if (count) { selected+=delta*r;if (selected<0) selected=0;if (selected>=count) selected=count-1; }
}
static void raise_window(int app) {
    if (focus!=app) { notice=0;save_confirm=0; }
    int at=0;while (at<APPS && order[at]!=app) at++;
    for (int i=at;i<APPS-1;i++) order[i]=order[i+1];
    order[APPS-1]=app;windows[app].open=1;windows[app].minimized=0;focus=app;
}
static void refocus(void) {
    focus=-1;for (int i=APPS-1;i>=0;i--) if (windows[order[i]].open && !windows[order[i]].minimized) { focus=order[i];break; }
}
static void notes_save(void) {
    if (!fat_mounted() || !fat_writable()) { notice="Mount a disk rw to save notes.";return; }
    struct fat_dirent e;
    if (!fat_lookup("desktop.txt",&e) && !save_confirm) { notice="Ctrl+S again replaces desktop.txt";save_confirm=1;return; }
    int r=fat_write_file("desktop.txt",notes,notes_len);
    notice=r?fat_strerror(r):"Saved desktop.txt in current folder";save_confirm=0;
}
static void content(int app,struct window *w) {
    if (app==0) {
        line(w,0,"Welcome to IronOS");line(w,2,"Drag title bars to move windows.");
        line(w,3,"_ minimizes; X closes. Tab focuses.");line(w,4,"Top bar opens apps; bottom restores.");
        line(w,6,"Ctrl+G: 3D demo | Esc: terminal");
        line(w,7,"Ctrl+H/F/S/N/A/C/O: open apps");
        line(w,9,"Apps are cooperative kernel code.");
    } else if (app==1) {
        int r=rows(w),cols=columns(w),first=selected/r*r;
        if (previewing) {
            int row=0,col=0,start=preview_page*r;
            for (int i=0;i<preview_len && row<start+r;i++) {
                unsigned char c=preview[i];if (c=='\r') continue;
                if (c=='\n') { row++;col=0;continue; }
                if (row>=start) { char s[2]={c>=32 && c<127?c:' ',0};text(w->x+8+col*8,w->y+bar+6+(row-start)*(fh+4),s,1); }
                if (++col==cols) { row++;col=0; }
            }
        } else {
            for (int i=first;i<count && i<first+r;i++) {
                int y=w->y+bar+6+(i-first)*(fh+4);
                if (i==selected) graphics_rect(w->x+5,y,w->w-10,fh+2,228);
                text(w->x+8,y,files[i].attr&FAT_ATTR_DIR?">":" ",1);text(w->x+24,y,files[i].name,cols-2);
            }
            if (!count) line(w,0,fat_mounted()?"Folder empty":"No filesystem mounted");
        }
        int bw=(w->w-20)/4,y=w->y+w->h-bar-2;
        button(w->x+4,y,bw,bar,"Back",0);button(w->x+8+bw,y,bw,bar,"Prev",0);
        button(w->x+12+bw*2,y,bw,bar,"Next",0);button(w->x+16+bw*3,y,bw,bar,"Open",0);
    } else if (app==2) {
        number_line(w,0,"Memory MiB: ",pmm_total_pages()/256);number_line(w,1,"Free MiB: ",pmm_free_page_count()/256);
        number_line(w,2,"Disks: ",disk_count());number_line(w,3,"Uptime seconds: ",uptime_ticks/TIMER_HZ);
        line(w,5,sw==800?"800 x 600 | 32-bit framebuffer":"320 x 200 | VGA fallback");
        line(w,6,"CPU rendering; no GPU acceleration");line(w,8,"32-bit x86 | BIOS | PS/2 mouse");
    } else if (app==3) {
        struct net_info info;net_get_info(&info);char ip[20];
        line(w,0,nic_name());line(w,1,nic_link()?"Ethernet link: up":"Ethernet link: down");
        net_format_ip(info.ip,ip);line(w,2,ip);net_format_ip(info.mask,ip);line(w,3,ip);
        net_format_ip(info.gateway,ip);line(w,4,ip);net_format_ip(info.dns,ip);line(w,5,ip);
        line(w,6,info.configured?(info.manual?"Static IP / mask / gateway / DNS":"DHCP IP / mask / gateway / DNS"):"Unconfigured: press D for DHCP");
        number_line(w,8,"Received packets: ",info.rx);number_line(w,9,"Sent packets: ",info.tx);
        line(w,11,"D: DHCP | X: clear IP settings");line(w,12,"Wi-Fi/USB drivers unavailable");
        int y=w->y+w->h-bar-2;button(w->x+6,y,100,bar,"DHCP [D]",0);button(w->x+112,y,130,bar,"Disconnect [X]",0);
    } else if (app==4) {
        line(w,0,"Integer calculator");line(w,2,expression);line(w,3,answer);
        line(w,5,"Type + - * / % and parentheses.");line(w,6,"Enter: calculate | Ctrl+L: clear");
        line(w,8,"Overflow / divide by zero checked.");
    } else if (app==5) {
        struct rtc_time t;
        if (rtc_get(&t)) line(w,1,"Clock unavailable");
        else { char b[80],n[16];unsigned int v[]={t.year,t.month,t.day,t.hour,t.minute,t.second};int at=0;
            for (int i=0;i<6;i++) { int_to_string(v[i],n);if (i && v[i]<10) b[at++]='0';for (int j=0;n[j];j++) b[at++]=n[j];if (i<5) b[at++]=i<2?'-':i==2?' ':':'; }b[at]=0;line(w,1,b);
        }
        number_line(w,3,"Uptime seconds: ",uptime_ticks/TIMER_HZ);line(w,5,"RTC hardware time");
    } else if (app==6) {
        int cols=columns(w),r=rows(w)-1,row=0,col=0,total=1;
        for (int i=0;i<notes_len;i++) { if (notes[i]=='\n' || ++col==cols) { total++;col=0; } }
        int first=total>r?total-r:0;col=0;
        for (int i=0;i<notes_len;i++) {
            if (notes[i]=='\n') { row++;col=0;continue; }
            if (row>=first && row<first+r) { char s[2]={notes[i],0};text(w->x+8+col*8,w->y+bar+6+(row-first)*(fh+4),s,1); }
            if (++col==cols) { row++;col=0; }
        }
        text(w->x+8,w->y+w->h-bar-2,"Ctrl+S: save as desktop.txt",(w->w-16)/8);
    }
}
static void draw(void) {
    graphics_rect(0,0,sw,sh,224);
    for (int y=bar;y<sh-bar;y+=32) graphics_rect(0,y,sw,1,225);
    graphics_rect(0,0,sw,bar,226);
    int appw=sw/APPS;
    for (int i=0;i<APPS;i++) button(i*appw,0,appw-2,bar,names[i],focus==i);
    for (int z=0;z<APPS;z++) {
        int app=order[z];struct window *w=&windows[app];if (!w->open || w->minimized) continue;
        graphics_rect(w->x+3,w->y+3,w->w,w->h,234);graphics_rect(w->x,w->y,w->w,w->h,225);
        graphics_rect(w->x,w->y,w->w,bar,focus==app?228:226);
        text(w->x+6,w->y+3,app==1?(previewing?files[selected].name:fat_getcwd()):names[app],(w->w-56)/8);
        button(w->x+w->w-42,w->y,20,bar,"_",0);button(w->x+w->w-21,w->y,20,bar,"X",0);
        content(app,w);
    }
    graphics_rect(0,sh-bar,sw,bar,226);
    int n=0;for (int i=0;i<APPS;i++) if (windows[i].open) { button(n*appw,sh-bar,appw-2,bar,names[i],focus==i);n++; }
    if (notice && focus>=0) { struct window *w=&windows[focus];int y=w->y+w->h-bar*2;graphics_rect(w->x+4,y,w->w-8,bar,227);text(w->x+8,y+2,notice,(w->w-16)/8); }
    if (mouse_present()) for (int y=0;y<12;y++) for (int x=0;x<=y/2;x++) graphics_rect(mx+x,my+y,1,1,!x || x==y/2?234:232);
    graphics_present();
}
/* 1 terminal, 2 game, 3 DHCP. */
static int click(void) {
    int appw=sw/APPS;
    if (my<bar) { int app=mx/appw;if (app<APPS) { raise_window(app);notice=0;if (app==1) refresh(); }return 0; }
    if (my>=sh-bar) {
        int n=0;for (int i=0;i<APPS;i++) if (windows[i].open) { if (mx/appw==n) { raise_window(i);break; }n++; }return 0;
    }
    for (int z=APPS-1;z>=0;z--) {
        int a=order[z];struct window *w=&windows[a];
        if (!w->open || w->minimized || !inside(mx,my,w->x,w->y,w->w,w->h)) continue;
        raise_window(a);notice=0;
        if (my<w->y+bar) {
            if (mx>=w->x+w->w-21) { w->open=0;refocus(); }
            else if (mx>=w->x+w->w-42) { w->minimized=1;refocus(); }
            else { drag=a;drag_x=mx-w->x;drag_y=my-w->y; }
        } else if (a==1) {
            if (my>=w->y+w->h-bar-2) {
                int b=(mx-w->x-4)/((w->w-20)/4+4);
                if (b==0) parent();else if (b==1) page(-1);else if (b==2) page(1);else if (b==3 && !previewing) open_file();
            } else if (!previewing) {
                int r=(my-w->y-bar-6)/(fh+4);int i=selected/rows(w)*rows(w)+r;
                if (my>=w->y+bar+6 && r<rows(w) && i<count) {
                    selected=i;if (last_click==i && uptime_ticks-last_click_time<TIMER_HZ/2) { open_file();last_click=-1; }
                    else { last_click=i;last_click_time=uptime_ticks; }
                }
            }
        } else if (a==3 && my>=w->y+w->h-bar-2) {
            if (mx<w->x+110) return 3;
            net_command("netoff","");
        }
        return 0;
    }
    return 0;
}
static int keypress(int k) {
    if (focus==6 && k==KEY_CTRL('s')) { notes_save();return 0; }
    if (k==27) { if (focus==1 && previewing) { parent();return 0; }return 1; }
    if (k==KEY_CTRL('g')) return 2;
    const char *shortcuts="hfsnaco";
    for (int i=0;i<APPS;i++) if (k==KEY_CTRL(shortcuts[i])) { raise_window(i);notice=0;if (i==1) refresh();return 0; }
    if (k=='\t') {
        for (int step=1;step<=APPS;step++) {
            int app=(focus+step)%APPS;
            if (windows[app].open && !windows[app].minimized) { raise_window(app);break; }
        }
        return 0;
    }
    if (focus==1) {
        if (k=='\b') parent();
        if (k=='\n' && !previewing) open_file();
        if (k==KEY_PGUP || (previewing && k==KEY_UP)) page(-1);
        if (k==KEY_PGDN || (previewing && k==KEY_DOWN)) page(1);
        if (!previewing && k==KEY_UP && selected) selected--;
        if (!previewing && k==KEY_DOWN && selected+1<count) selected++;
    } else if (focus==3) {
        if (k=='d' || k=='D') return 3;
        if (k=='x' || k=='X') net_command("netoff","");
    } else if (focus==4) {
        if (k=='\n') { int n;if (calc_eval(expression,&n)) memcpy(answer,"Invalid / overflow / divide by zero",sizeof("Invalid / overflow / divide by zero"));else signed_string(n,answer); }
        else if (k==KEY_CTRL('l')) { expr_len=0;expression[0]=answer[0]=0; }
        else if (k=='\b' && expr_len) expression[--expr_len]=0;
        else if (k>=32 && k<127 && expr_len<95) { expression[expr_len++]=k;expression[expr_len]=0; }
    } else if (focus==6) {
        if (k==KEY_CTRL('s')) notes_save();
        else if (k=='\b' && notes_len) { notes[--notes_len]=0;save_confirm=0;notice=0; }
        else if ((k=='\n' || (k>=32 && k<127)) && notes_len<2047) { notes[notes_len++]=k;notes[notes_len]=0;save_confirm=0;notice=0; }
    }
    return 0;
}
static void begin(void) {
    graphics_begin_desktop();sw=graphics_width();sh=graphics_height();fh=graphics_font_height();bar=fh+6;
    struct mouse_event e;while (mouse_get_event(&e)) {}mouse_get_state(&e);mx=e.x;my=e.y;
}
void gui_run(void) {
    char saved_dir[256];int restore=fat_mounted();if (restore) memcpy(saved_dir,fat_getcwd(),strlen(fat_getcwd())+1);
    begin();
    for (int i=0;i<APPS;i++) {
        int w=sw==800?472:292,h=sw==800?352:154;
        windows[i]=(struct window){sw==800?70+i*20:10,sw==800?50+i*20:18,w,h,0,0};order[i]=i;
    }
    raise_window(0);drag=-1;notice=0;unsigned long last=uptime_ticks;int dirty=1;
    for (;;) {
        net_poll();int action=0;struct mouse_event e;
        while (mouse_get_event(&e)) {
            mx=e.x;my=e.y;dirty=1;
            if (!(e.buttons&1)) drag=-1;
            if (drag>=0) {
                struct window *w=&windows[drag];w->x=mx-drag_x;w->y=my-drag_y;
                if (w->x<0) w->x=0;
                if (w->x>sw-w->w) w->x=sw-w->w;
                if (w->y<bar) w->y=bar;
                if (w->y>sh-bar-w->h) w->y=sh-bar-w->h;
            }
            if (e.pressed&1) action=click();
            if (action) break;
        }
        int k=keyboard_getchar();if (k>=0) { dirty=1;int a=keypress(k);if (a) action=a; }
        if (action==1) break;
        if (action==2 || action==3) {
            drag=-1;graphics_end();
            if (action==2) graphics_demo();else net_command("dhcp","");
            begin();dirty=1;
        }
        if (uptime_ticks/TIMER_HZ!=last/TIMER_HZ) dirty=1;
        if (dirty && uptime_ticks-last>=3) { draw();dirty=0;last=uptime_ticks; }
        halt();
    }
    graphics_end();if (restore) fat_chdir(saved_dir);
}
