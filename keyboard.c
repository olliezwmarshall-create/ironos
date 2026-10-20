#include "kernel.h"

#define KB_BUF_SIZE 128

static const unsigned char keymap[0x3A] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t', 'q','w','e','r','t','y','u','i','o','p','[',']', '\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\', 'z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' '
};

static const unsigned char keymap_shift[0x3A] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t', 'Q','W','E','R','T','Y','U','I','O','P','{','}', '\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|', 'Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' '
};

static volatile int kb_buf[KB_BUF_SIZE];
static volatile int kb_head = 0;   // written by the IRQ
static volatile int kb_tail = 0;   // written by the reader
static int shift_down = 0, caps_on = 0, ctrl_down = 0, extended = 0;

static void kb_push(int key) {
    int next = (kb_head + 1) % KB_BUF_SIZE;
    if (next != kb_tail) {          // drop keys if the buffer is full
        kb_buf[kb_head] = key;
        kb_head = next;
    }
}

static void keyboard_callback(struct registers *r) {
    (void)r;
    unsigned char sc = inb(0x60);

    if (sc == 0xE0) { extended = 1; return; }

    int released = sc & 0x80;
    sc &= 0x7F;

    if (extended) {                 // arrow keys etc. arrive as E0 xx
        extended = 0;
        if (sc == 0x1D) { ctrl_down = !released; return; }       // right Ctrl
        if (!released) {
            switch (sc) {
                case 0x48: kb_push(KEY_UP);     break;
                case 0x50: kb_push(KEY_DOWN);   break;
                case 0x4B: kb_push(KEY_LEFT);   break;
                case 0x4D: kb_push(KEY_RIGHT);  break;
                case 0x47: kb_push(KEY_HOME);   break;
                case 0x4F: kb_push(KEY_END);    break;
                case 0x49: kb_push(KEY_PGUP);   break;
                case 0x51: kb_push(KEY_PGDN);   break;
                case 0x53: kb_push(KEY_DELETE); break;
            }
        }
        return;
    }

    if (sc == 0x1D) { ctrl_down = !released; return; }           // left Ctrl
    if (sc == 0x2A || sc == 0x36) { shift_down = !released; return; }
    if (sc == 0x3A && !released)  { caps_on = !caps_on; return; }
    if (released || sc >= 0x3A) return;

    unsigned char base = keymap[sc];
    int is_letter = (base >= 'a' && base <= 'z');
    int upper = is_letter ? (shift_down ^ caps_on) : shift_down;
    unsigned char c = upper ? keymap_shift[sc] : base;

    if (ctrl_down && is_letter) { kb_push(KEY_CTRL(base)); return; }   // Ctrl+letter
    if (c) kb_push(c);
}

void keyboard_init(void) {
    register_interrupt_handler(33, keyboard_callback);   // IRQ1
}

int keyboard_getchar(void) {
    if (kb_head == kb_tail) return -1;
    int key = kb_buf[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return key;
}
